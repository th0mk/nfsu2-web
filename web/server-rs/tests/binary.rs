//! The whole server as a process (cargo's CARGO_BIN_EXE_nfsu2-server), with a made-up game folder as
//! web/test/module.test.mjs makes one: its start-up lines, /players counting a /lan client and an Online login, what
//! the console says of them, and SIGTERM (exit code 0, the addresses saved); its state folder over restarts; other
//! options; the lobby trace; players who leave nothing behind; a page's first messages and its replaced connection;
//! kicks and bans through `nfsu2-server ctl`, and the control socket's file.
//!   cargo test --test binary

mod common;

use std::io::{BufRead, BufReader};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use common::*;
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

/// module.test.mjs's game folder (SPEED2.EXE, "MZ") and a module beside it
fn game_folder(root: &Path) -> (PathBuf, PathBuf) {
    let data = root.join("game");
    std::fs::create_dir_all(&data).unwrap();
    std::fs::write(data.join("SPEED2.EXE"), "MZ").unwrap();
    let wasm = root.join("game.wasm");
    std::fs::write(&wasm, vec![7u8; 4096]).unwrap();
    (data, wasm)
}

/// A server process: what it printed (stdout and stderr, line by line, in the order each stream had them)
struct Server {
    child: Child,
    out: Arc<Mutex<Vec<String>>>,
    port: u16,
}

impl Server {
    fn spawn(mut cmd: Command, port: u16) -> Server {
        let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().expect("the server starts");
        let out = Arc::new(Mutex::new(Vec::new()));
        for s in [Box::new(child.stdout.take().unwrap()) as Box<dyn std::io::Read + Send>, Box::new(child.stderr.take().unwrap())] {
            let out = out.clone();
            std::thread::spawn(move || {
                for l in BufReader::new(s).lines().map_while(Result::ok) {
                    out.lock().unwrap().push(l);
                }
            });
        }
        Server { child, out, port }
    }

    fn lines(&self) -> Vec<String> {
        self.out.lock().unwrap().clone()
    }

    /// the tests' sign that it listens: "  access    :"
    async fn ready(&mut self) {
        let t0 = Instant::now();
        while !self.lines().iter().any(|l| l.starts_with("  access    :")) {
            if let Ok(Some(st)) = self.child.try_wait() {
                panic!("the server exited ({st}): {:?}", self.lines());
            }
            assert!(t0.elapsed() < Duration::from_secs(30), "the server did not start: {:?}", self.lines());
            sleep(20).await;
        }
    }

    /// GET `path` (Host: localhost): the answer's head and its body
    async fn get(&self, path: &str) -> (String, String) {
        let mut s = tokio::net::TcpStream::connect(("127.0.0.1", self.port)).await.unwrap();
        let q = format!("GET {path} HTTP/1.1\r\nHost: localhost:{}\r\nConnection: close\r\n\r\n", self.port);
        s.write_all(q.as_bytes()).await.unwrap();
        let mut r = Vec::new();
        s.read_to_end(&mut r).await.unwrap();
        let r = String::from_utf8_lossy(&r).to_string();
        assert!(r.starts_with("HTTP/1.1 200 "), "{r}");
        let (head, body) = r.split_once("\r\n\r\n").unwrap();
        (head.to_string(), body.to_string())
    }

    /// GET /players as the start page asks: its body
    async fn players(&self) -> String {
        self.get("/players").await.1
    }

    /// /players until it says `players` players (2 s at most)
    async fn players_until(&self, players: u64) -> String {
        let t0 = Instant::now();
        loop {
            let p = self.players().await;
            let v: Value = serde_json::from_str(&p).unwrap();
            if v["players"] == players || t0.elapsed() > Duration::from_secs(2) {
                return p;
            }
            sleep(20).await;
        }
    }

    /// /players until it counts `roam` in free roam (as of the hub's last tick: twice a second; 2 s at most)
    async fn roam_until(&self, roam: u64) -> String {
        let t0 = Instant::now();
        loop {
            let p = self.players().await;
            let v: Value = serde_json::from_str(&p).unwrap();
            if v["roam"] == roam || t0.elapsed() > Duration::from_secs(2) {
                return p;
            }
            sleep(20).await;
        }
    }

    /// SIGTERM, and its exit code
    async fn stop(&mut self) -> Option<i32> {
        // (kill(1): no unsafe outside nfsu2-util)
        let _ = Command::new("kill").args(["-TERM", &self.child.id().to_string()]).status();
        let t0 = Instant::now();
        loop {
            if let Some(st) = self.child.try_wait().unwrap() {
                return st.code();
            }
            if t0.elapsed() > Duration::from_secs(10) {
                let _ = self.child.kill();
                return None;
            }
            sleep(20).await;
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// What the server did in the scenario
#[derive(Debug)]
struct Run {
    /// the start-up lines, from "NFSU2 web server" to "  access    :"
    startup: Vec<String>,
    /// /players: at the start, with a /lan client on, once it logged in to Online mode, once it left; then with a
    /// room (?room=5) and with one that is none (?room=x)
    players: Vec<String>,
    /// what the console said after the start-up lines (sorted: the switch and the Online host print from threads of
    /// their own)
    console: Vec<String>,
    exit: Option<i32>,
    /// lan-addresses.json's addresses after the stop, the state folder's mode and the online folder's
    addresses: Vec<String>,
    modes: (u32, u32),
}

/// the scenario against the server `cmd` starts on `port` with the state folder `state`
async fn scenario(cmd: Command, port: u16, state: &Path) -> Run {
    let mut srv = Server::spawn(cmd, port);
    srv.ready().await;
    let mut players = vec![srv.players().await];
    let a = Browser::open_at(&format!("localhost:{port}"), port, "", None).await;
    a.hello("Ann");
    players.push(srv.players_until(1).await);
    a.login("Ann").await;
    players.push(srv.players().await);
    a.close().await;
    players.push(srv.players_until(0).await);
    players.push(srv.get("/players?room=5").await.1);
    players.push(srv.get("/players?room=x").await.1);
    // the lines the console printed by now (the lobby session's end comes a moment after the switch's)
    sleep(300).await;
    let exit = srv.stop().await;
    sleep(100).await;
    let lines = srv.lines();
    let first = lines.iter().position(|l| l == "NFSU2 web server").expect("the start-up lines");
    let last = lines.iter().position(|l| l.starts_with("  access    :")).unwrap();
    let mut console: Vec<String> = lines[last + 1..].to_vec();
    console.sort();
    let store: Value =
        serde_json::from_str(&std::fs::read_to_string(state.join("lan-addresses.json")).unwrap_or_else(|_| "[]".into())).unwrap();
    let addresses = store.as_array().map_or(Vec::new(), |l| l.iter().map(|e| e["v"].as_str().unwrap_or("").to_string()).collect());
    let mode = |p: &Path| std::fs::metadata(p).map_or(0, |m| m.permissions().mode() & 0o777);
    Run { startup: lines[first..=last].to_vec(), players, console, exit, addresses, modes: (mode(state), mode(&state.join("online"))) }
}

/// a line's time ("... in 12 ms") taken out
fn untimed(l: String) -> String {
    match l.strip_suffix(" ms").and_then(|s| s.rsplit_once(" in ")) {
        Some((head, t)) if t.bytes().all(|c| c.is_ascii_digit()) => format!("{head} in <n> ms"),
        _ => l,
    }
}

/// a run with its own paths, port and times taken out
fn normal(mut r: Run, port: u16, state: &Path) -> Run {
    let st = state.to_string_lossy().to_string();
    // (the server names some folders by their real path: on macOS the temp folder is under a link, /var to /private/var)
    let real = std::fs::canonicalize(state).map_or_else(|_| st.clone(), |p| p.to_string_lossy().to_string());
    let fix = |l: &String| untimed(l.replace(&real, "<state>").replace(&st, "<state>").replace(&format!(":{port}/"), ":<port>/"));
    r.startup = r.startup.iter().map(fix).collect();
    r.console = r.console.iter().map(fix).collect();
    r
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_binary_starts_counts_its_players_and_stops() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let args = |port: u16, state: &Path| {
        vec![
            "--data".to_string(),
            data.to_string_lossy().into(),
            "--http-port".into(),
            port.to_string(),
            "--state".into(),
            state.to_string_lossy().into(),
            "--wasm".into(),
            wasm.to_string_lossy().into(),
        ]
    };
    let (port, state) = (free_port(), tmp.path().join("state"));
    let mut cmd = Command::new(env!("CARGO_BIN_EXE_nfsu2-server"));
    cmd.args(args(port, &state)).env("NFSU2_THREADS", "2");
    let rust = normal(scenario(cmd, port, &state).await, port, &state);
    println!("{rust:#?}");

    let data_s = data.to_string_lossy();
    assert_eq!(rust.startup[0], "NFSU2 web server");
    assert!(rust.startup[1].starts_with(&format!("  game data : {data_s} (")), "{}", rust.startup[1]);
    assert_eq!(rust.startup[2], format!("  game.wasm : {}", wasm.to_string_lossy()));
    // (run.rs names the machine: "this Mac" on macOS)
    let here = if cfg!(target_os = "macos") { "this Mac" } else { "this host" };
    assert_eq!(rust.startup[3], format!("  {here:<10}: http://localhost:<port>/"));
    assert!(rust.startup.iter().any(|l| l.starts_with("  free roam : the hub at 10.77.0.253 on the switch")));
    assert!(rust.startup.iter().any(|l| l.starts_with("  online    : the game's Online mode at 10.77.0.1")
        && l.ends_with("race results per persona in <state>/online)")));
    let p: Vec<Value> = rust.players.iter().map(|p| serde_json::from_str(p).unwrap()).collect();
    assert_eq!(rust.players[0], r#"{"players":0,"online":0,"roam":0,"full":false,"queue":0,"roamBusy":false}"#);
    assert_eq!((p[1]["players"].as_u64(), p[1]["online"].as_u64()), (Some(1), Some(0)), "a /lan client: {}", rust.players[1]);
    assert_eq!((p[2]["players"].as_u64(), p[2]["online"].as_u64()), (Some(1), Some(1)), "an Online login: {}", rust.players[2]);
    assert_eq!(rust.players[3], r#"{"players":0,"online":0,"roam":0,"full":false,"queue":0,"roamBusy":false}"#);
    assert_eq!(rust.players[4], r#"{"players":0,"online":0,"roam":0,"full":false,"queue":0,"roamBusy":false,"room":0}"#);
    assert_eq!(rust.players[5], rust.players[3], "no room for one that is not a number");
    assert!(rust.console.iter().any(|l| l == "[lan] 10.77.0.2 joined (1 on the switch)"), "{:?}", rust.console);
    assert!(rust.console.iter().any(|l| l == "[lan] 10.77.0.2 \"Ann\" left (0 on the switch)"), "{:?}", rust.console);
    assert!(rust.console.iter().any(|l| l.contains("logged in as \"Ann\"")), "{:?}", rust.console);
    assert_eq!(rust.exit, Some(0), "SIGTERM: exit code 0");
    assert_eq!(rust.addresses, ["10.77.0.2"], "the departed player's address kept for its token, saved at the stop");
    // (the online folder is made with its first file: none here, no race and no friend)
    assert_eq!(rust.modes.0, 0o700, "the state folder is private");
    assert!(rust.modes.1 == 0 || rust.modes.1 == 0o700, "the online folder is private");
    // keyphrase saves: their folder (private too), counted at the start
    assert!(
        rust.startup.contains(&"  saves     : keyphrase saves (/vault) in <state>/vault, up to 50.00 GB (new keyphrases until 45.00 GB, growing 1.50 GB a day at most); one unused for two years goes".to_string()),
        "{:?}",
        rust.startup
    );
    assert!(rust.console.iter().any(|l| l.starts_with("[vault] 0 keyphrases, 0.0 MB of 50.00 GB (counted in ")), "{:?}", rust.console);
    assert_eq!(
        std::fs::metadata(state.join("vault")).map(|m| m.permissions().mode() & 0o777).ok(),
        Some(0o700),
        "the keyphrases' folder is private"
    );
    // the kept blocks: their folder, private too
    assert!(
        rust.startup.contains(&"  blocks    : kept at brotli's best in <state>/blocks, 2 threads (NFSU2_COMPRESS_THREADS)".to_string()),
        "{:?}",
        rust.startup
    );
    assert_eq!(
        std::fs::metadata(state.join("blocks")).map(|m| m.permissions().mode() & 0o777).ok(),
        Some(0o700),
        "the kept blocks' folder is private"
    );
}

/// a header of an answer's head (its name in any case)
fn header(head: &str, name: &str) -> Option<String> {
    head.split("\r\n").filter_map(|l| l.split_once(':')).find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.trim().to_string())
}

// One state folder over two restarts: a player's address comes back to its token after each (lan-addresses.json), and
// the ETags stay the same (etag.key), so the players keep their places and their browsers their copies.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_state_folder_carries_over_a_restart() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let state = tmp.path().join("state");
    let (mut vip, mut token, mut etag) = (0, String::new(), None);
    for (i, which) in ["first run", "second run", "third run"].into_iter().enumerate() {
        let port = free_port();
        let mut srv = Server::spawn(command(&data, &wasm, port, &state, &[]), port);
        srv.ready().await;
        let e = header(&srv.get("/data-manifest").await.0, "etag");
        assert!(e.is_some(), "{which}: /data-manifest has an ETag");
        let host = format!("localhost:{port}");
        let a = if i == 0 {
            etag = e;
            let a = Browser::open_at(&host, port, "", None).await;
            a.hello("Ann");
            vip = a.vip;
            a
        } else {
            assert_eq!(e, etag, "{which}: the same ETag with the key of before");
            let a = Browser::resume(&host, port, "Ann", vip, &token).await;
            assert_eq!(nfsu2_util::vip_str(a.vip), nfsu2_util::vip_str(vip), "{which}: the address came back to its token");
            a
        };
        token = a.token.clone();
        sleep(200).await;
        assert_eq!(srv.stop().await, Some(0), "{which}: SIGTERM, exit code 0");
        drop(a);
        let lines = srv.lines();
        let back = format!("[lan] {} is back (1 on the switch)", nfsu2_util::vip_str(vip));
        assert!(i == 0 || lines.contains(&back), "{which}: {lines:?}");
    }
}

/// the server's command, with the game folder, a port and a state folder
fn command(data: &Path, wasm: &Path, port: u16, state: &Path, extra: &[&str]) -> Command {
    let mut c = Command::new(env!("CARGO_BIN_EXE_nfsu2-server"));
    c.env("NFSU2_THREADS", "2");
    c.arg("--data").arg(data).arg("--http-port").arg(port.to_string()).arg("--state").arg(state).arg("--wasm").arg(wasm);
    c.args(extra);
    c
}

/// What the server with --online off --roam-hub off --max-peers 1 did
#[derive(Debug)]
struct SmallRun {
    startup: Vec<String>,
    /// /players at the start, with one player on (full), after the second was refused
    players: Vec<String>,
    /// what the second client got: its messages and its close
    refused: (Vec<Value>, Option<(u16, String)>),
    /// what the first one got back for frames to 10.77.0.1 and 10.77.0.253 (no hosts there)
    answers: usize,
    exit: Option<i32>,
}

async fn small(cmd: Command, port: u16, state: &Path) -> SmallRun {
    let mut srv = Server::spawn(cmd, port);
    srv.ready().await;
    let host = format!("localhost:{port}");
    let mut players = vec![srv.players().await];
    let a = Browser::open_at(&host, port, "", None).await;
    a.hello("Ann");
    players.push(srv.players_until(1).await);
    let refused = Browser::refused(&host, port).await;
    let before = a.got().frame_count;
    a.tcp_out(Conn { port: 50000, cid: 1, dport: nfsu2_online::PORT_LOBBY }, SYN, &[]);
    a.binary(to_hub(&roam_hello(1)));
    sleep(300).await;
    let answers = a.got().frame_count - before;
    players.push(srv.players().await);
    let exit = srv.stop().await;
    sleep(100).await;
    let lines = srv.lines();
    let first = lines.iter().position(|l| l == "NFSU2 web server").expect("the start-up lines");
    let last = lines.iter().position(|l| l.starts_with("  access    :")).unwrap();
    let st = state.to_string_lossy().to_string();
    let fix = |l: &String| untimed(l.replace(&st, "<state>").replace(&format!(":{port}/"), ":<port>/"));
    SmallRun { startup: lines[first..=last].iter().map(fix).collect(), players, refused, answers, exit }
}

// (module.test.mjs's way, other options) --online off --roam-hub off --max-peers 1: the start-up lines say so,
// /players says online: null and full once one plays, a second client gets "LAN full", nothing answers at
// 10.77.0.1 or 10.77.0.253
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_binary_with_online_and_the_hub_off_and_one_place() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let extra = ["--online", "off", "--roam-hub", "off", "--max-peers", "1"];
    let (port, state) = (free_port(), tmp.path().join("state"));
    let rust = small(command(&data, &wasm, port, &state, &extra), port, &state).await;
    println!("{rust:#?}");
    assert!(rust.startup.contains(&"  online    : off (--online off)".to_string()), "{:?}", rust.startup);
    assert!(rust.startup.contains(&"  switch    : at most 1 players".to_string()));
    assert_eq!(rust.players[0], r#"{"players":0,"online":null,"roam":null,"full":false,"queue":0,"roamBusy":false}"#);
    assert_eq!(rust.players[1], r#"{"players":1,"online":null,"roam":null,"full":true,"queue":0,"roamBusy":false}"#);
    assert_eq!(rust.players[2], rust.players[1]);
    assert_eq!(rust.refused.1, Some((1013, "LAN full".to_string())));
    assert_eq!(rust.answers, 0, "no host at 10.77.0.1 or 10.77.0.253");
    assert_eq!(rust.exit, Some(0));
}

/// the status of GET `path` (Host: localhost)
async fn status_of(port: u16, path: &str) -> u16 {
    let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let q = format!("GET {path} HTTP/1.1\r\nHost: localhost:{port}\r\nConnection: close\r\n\r\n");
    s.write_all(q.as_bytes()).await.unwrap();
    let mut r = Vec::new();
    s.read_to_end(&mut r).await.unwrap();
    String::from_utf8_lossy(&r).get(9..12).and_then(|c| c.parse().ok()).unwrap_or(0)
}

/// what the binary prints with these arguments, and its exit code
fn printed(args: &[&str]) -> (String, String, Option<i32>) {
    let o = Command::new(env!("CARGO_BIN_EXE_nfsu2-server")).args(args).output().expect("the binary runs");
    (String::from_utf8_lossy(&o.stdout).into_owned(), String::from_utf8_lossy(&o.stderr).into_owned(), o.status.code())
}

// --game mw on a made-up Most Wanted folder (its program in lower case, beside a mod loader, a widescreen fix's
// scripts and the SAVE folder it writes profiles into): the start-up lines, no Online mode (the game has none yet),
// of the folder only the game's own entries listed and served; --print-entries for both games, with no folder; a
// folder of the other game refused
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_binary_for_most_wanted() {
    let tmp = tempfile::tempdir().unwrap();
    let data = tmp.path().join("mw");
    for d in ["GLOBAL", "SOUND/PFDATA", "SAVE", "scripts"] {
        std::fs::create_dir_all(data.join(d)).unwrap();
    }
    for (f, text) in [
        ("speed.exe", "MZ"),
        ("GLOBAL/a.bin", "a"),
        ("SOUND/PFDATA/m.mus", "m"),
        ("SAVE/profile", "SECRET"),
        ("scripts/fix.ini", "SECRET"),
        ("dinput8.dll", "SECRET"),
        ("foobar", ""),
    ] {
        std::fs::write(data.join(f), text).unwrap();
    }
    let wasm = tmp.path().join("nfsmw.wasm");
    std::fs::write(&wasm, vec![7u8; 4096]).unwrap();
    let (port, state) = (free_port(), tmp.path().join("state"));
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &["--game", "mw"]), port);
    srv.ready().await;
    let lines = srv.lines();
    let first = lines.iter().position(|l| l == "NFSMW web server").expect("the start-up lines");
    let startup = &lines[first..];
    assert_eq!(startup[1], format!("  game data : {} (6 entries of the game's own)", data.to_string_lossy()), "{startup:?}");
    assert_eq!(startup[2], format!("  game.wasm : {}", wasm.to_string_lossy()));
    assert!(startup.contains(&"  online    : off (this server has no Online mode for NFS Most Wanted yet)".to_string()), "{startup:?}");
    assert!(startup.iter().any(|l| l.starts_with("  free roam : the hub at 10.77.0.253 on the switch")));
    // the operator's command names the game (ctl's own default is UG2's state folder)
    let sock = state.join("control.sock").to_string_lossy().to_string();
    let bans = format!("  bans      : 0; nfsu2-server ctl --game mw who, kick, ban, unban, bans (through {sock})");
    assert!(startup.contains(&bans), "{startup:?}");
    let (_, m) = srv.get("/data-manifest").await;
    let m: Value = serde_json::from_str(&m).unwrap();
    let ps: Vec<&str> = m["files"].as_array().unwrap().iter().map(|f| f["p"].as_str().unwrap()).collect();
    assert_eq!(ps, ["GLOBAL", "GLOBAL/a.bin", "SOUND", "SOUND/PFDATA", "SOUND/PFDATA/m.mus", "speed.exe"]);
    assert_eq!(srv.get("/data/speed.exe").await.1, "MZ");
    for p in ["/data/SAVE/profile", "/data/scripts/fix.ini", "/data/dinput8.dll", "/data/foobar", "/data/SPEED2.EXE"] {
        assert_eq!(status_of(port, p).await, 404, "{p}");
    }
    assert_eq!(srv.players().await, r#"{"players":0,"online":null,"roam":0,"full":false,"queue":0,"roamBusy":false}"#);
    assert_eq!(srv.stop().await, Some(0));

    // the entries each game's folder is served by, one a line, with no folder
    let ug2 = "speed2.exe server.dll server.cfg cars credits frontend global langpacks languages memcard movies nis pfdata sound speech \
               subtitles texpacks tracks";
    let mw = "speed.exe server.dll server.cfg cars credits frontend global languages memcard movies nis sound subtitles tracks";
    let cases: [(&[&str], &str); 3] =
        [(&["--print-entries"], ug2), (&["--game", "mw", "--print-entries"], mw), (&["--print-entries", "--game", "ug2"], ug2)];
    for (args, want) in cases {
        let (out, err, code) = printed(args);
        assert_eq!((out.lines().collect::<Vec<_>>(), err.as_str(), code), (want.split_whitespace().collect(), "", Some(0)), "{args:?}");
    }
    let (_, err, code) = printed(&["--game", "nfs6", "--print-entries"]);
    assert_eq!((err.trim_end(), code), ("--game: ug2 or mw", Some(1)));
    // the other game's folder: no Most Wanted program in it
    let (ug2_data, _) = game_folder(tmp.path());
    let ug2_data = ug2_data.to_string_lossy().into_owned();
    let (_, err, code) = printed(&["--game", "mw", "--data", &ug2_data, "--state", &state.to_string_lossy()]);
    let usage = format!("usage: nfsu2-server --game mw --data \"<folder with speed.exe>\"  (got: {ug2_data})");
    assert_eq!((err.trim_end(), code), (usage.as_str(), Some(1)));
}

/// the lobby trace's lines without their times (and the start line's date) and the game's own port, sorted: the
/// switch and the Online host write theirs from threads of their own
fn trace_lines(f: &Path, game_port: u16) -> Vec<String> {
    let mut l: Vec<String> = std::fs::read_to_string(f)
        .unwrap_or_default()
        .lines()
        .map(|l| l.split_once(' ').map_or(l, |x| x.1).replace(&format!(":{game_port} "), ":<port> "))
        .map(|l| if l.starts_with("--- trace started ") { "--- trace started <date>".into() } else { l })
        .collect();
    l.sort();
    l
}

// (ONL-12 through the binary) --dev --online-trace: an Online login's lobby messages in the trace, its LKEY
// redacted, the file 0600
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_lobby_trace_of_a_login() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let (port, state, trace) = (free_port(), tmp.path().join("state"), tmp.path().join("lobby.trace"));
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &["--dev", "--online-trace", trace.to_str().unwrap()]), port);
    srv.ready().await;
    assert!(srv.lines().iter().any(|l| l.starts_with("  trace     : lobby messages (secrets redacted) into ")));
    let a = Browser::open_at(&format!("localhost:{port}"), port, "", None).await;
    a.hello("Ann");
    let c = a.login("Ann").await;
    let lkey = a.tcp(c, |t| t.last("pers").and_then(|m| m.get("LKEY")).map(str::to_string));
    sleep(200).await;
    assert_eq!(srv.stop().await, Some(0));
    let mode = std::fs::metadata(&trace).unwrap().permissions().mode() & 0o777;
    let rust = trace_lines(&trace, c.port);
    println!("{rust:#?}");
    assert_eq!(mode, 0o600, "the trace is the server user's alone");
    let lkey = lkey.expect("the login's LKEY");
    assert!(!lkey.is_empty() && !rust.iter().any(|l| l.contains(&lkey)), "no LKEY in the trace");
    assert!(rust.iter().any(|l| l.contains("LKEY=<redacted>")), "{rust:?}");
    assert!(rust.iter().any(|l| l.contains(" auth/0 ")), "{rust:?}");
}

/// a process's open files, threads and resident memory (/proc/<pid>)
fn proc_use(pid: u32) -> (usize, usize, u64) {
    let fds = std::fs::read_dir(format!("/proc/{pid}/fd")).map_or(0, |d| d.count());
    let status = std::fs::read_to_string(format!("/proc/{pid}/status")).unwrap_or_default();
    let field = |k: &str| {
        status.lines().find(|l| l.starts_with(k)).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0)
    };
    (fds, field("Threads:") as usize, field("VmRSS:") * 1024)
}

// Players come and go through the whole binary, in rounds of 40 (each a client address of its own,
// as a proxy on this machine names it): a /lan connection, an Online login, free roam's HELLO and SNAP to the hub,
// then the page closes.  Once they left, /players says 0 and the process holds what it held after the first round:
// the same open files, about the same memory.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn players_who_leave_leave_nothing_behind() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let (port, state) = (free_port(), tmp.path().join("state"));
    // (each one that leaves without the page's own close keeps its place a minute: room for the four rounds' 160)
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &["--max-peers", "1000"]), port);
    srv.ready().await;
    let pid = srv.child.id();
    let host = format!("localhost:{port}");
    let mut seen: Vec<(usize, usize, u64)> = Vec::new();
    for round in 0..4 {
        let mut bs = Vec::new();
        for i in 0..40 {
            let b = Browser::open_from(&host, port, "", None, None, Some(&format!("10.{round}.0.{}", i + 1))).await;
            b.hello(&format!("P{round}x{i}"));
            bs.push(b);
        }
        for (i, b) in bs.iter().enumerate() {
            b.login(&format!("P{round}x{i}")).await;
            b.binary(to_hub(&roam_hello(1000 + i as u32)));
            b.binary(to_hub(&roam_snap(1000 + i as u32, [i as f32 * 10.0, 0.0, 0.0])));
        }
        // (all 40 in free roam once the hub's tick has counted their HELLOs: until then fewer, or none)
        let during = srv.roam_until(40).await;
        assert_eq!(during, r#"{"players":40,"online":40,"roam":40,"full":false,"queue":0,"roamBusy":false}"#);
        let used = proc_use(pid);
        for b in &bs {
            b.close().await;
        }
        drop(bs);
        assert_eq!(srv.players_until(0).await, r#"{"players":0,"online":0,"roam":0,"full":false,"queue":0,"roamBusy":false}"#);
        // (the sockets close as their tasks end: until the count holds, or the first round's again, 3 s at most)
        let t0 = Instant::now();
        let mut after = proc_use(pid);
        while t0.elapsed() < Duration::from_secs(3) {
            sleep(200).await;
            let now = proc_use(pid);
            let settled = if round == 0 { now.0 == after.0 } else { now.0 <= seen[0].0 };
            after = now;
            if settled {
                break;
            }
        }
        println!("round {round}: with 40 players {used:?}, after they left {after:?} (open files, threads, memory)");
        seen.push(after);
    }
    let (first, last) = (seen[0], seen[3]);
    assert_eq!(last.0, first.0, "the same open files after each round: {seen:?}");
    assert!(last.2 < first.2 + (16 << 20), "about the same memory: {seen:?}");
    assert_eq!(srv.stop().await, Some(0));
    assert!(!srv.lines().iter().any(|l| l.contains("panicked")), "{:?}", srv.lines());
}

/// a HELLO of a player that reads crews (roam_core.h RC_HF_CREW, core version 7) in `room`, in free roam, named "p",
/// naming no ids it hears, then its crews
fn roam_hello_crews(sender: u32, room: u32, crews: &[u32]) -> Vec<u8> {
    let mut p = vec![0u8; 12];
    p[0..2].copy_from_slice(&7u16.to_le_bytes());
    p[2] = 1;
    p[3] = 2;
    p[4..6].copy_from_slice(&47210u16.to_le_bytes());
    p[6..8].copy_from_slice(&3u16.to_le_bytes());
    p[8..12].copy_from_slice(&room.to_le_bytes());
    p.extend_from_slice(&[1, b'p', 0, crews.len() as u8]);
    for c in crews {
        p.extend_from_slice(&c.to_le_bytes());
    }
    let mut d = dgram(1, sender, &p);
    d[14..16].copy_from_slice(&0x0200u16.to_le_bytes());
    let n = d.len();
    let crc = crc32(&d[..n - 4]);
    d[n - 4..].copy_from_slice(&crc.to_le_bytes());
    d
}

// /players?crews=: how many drive in free roam in each crew asked, in any room, in the order asked, as of the hub's
// last tick; with ?room= too; what is not a number, or 0, left out, 4 at most; none left, no crews
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn players_counts_free_roams_crews() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let (port, state) = (free_port(), tmp.path().join("state"));
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &[]), port);
    srv.ready().await;
    let host = format!("localhost:{port}");
    let (a, b) = (Browser::open_at(&host, port, "", None).await, Browser::open_at(&host, port, "", None).await);
    a.hello("Ann");
    b.hello("Bob");
    a.binary(to_hub(&roam_hello_crews(501, 5, &[7, 8])));
    b.binary(to_hub(&roam_hello_crews(502, 0, &[8])));
    // /players with this query once the hub's tick counts `roam` in free roam
    let until = |q: &'static str, roam: u32| {
        let srv = &srv;
        async move {
            let t0 = Instant::now();
            loop {
                let p = srv.get(q).await.1;
                if p.contains(&format!("\"roam\":{roam},")) || t0.elapsed() > Duration::from_secs(5) {
                    return p;
                }
                sleep(50).await;
            }
        }
    };
    let head = r#"{"players":2,"online":0,"roam":2,"full":false,"queue":0,"roamBusy":false"#;
    assert_eq!(until("/players?room=5&crews=8,x,7,0,9,10,11", 2).await, format!(r#"{head},"room":1,"crews":[2,1,0,0]}}"#));
    assert_eq!(srv.get("/players?crews=7").await.1, format!(r#"{head},"crews":[1]}}"#));
    assert_eq!(srv.get("/players?crews=8%2C7&room=5").await.1, format!(r#"{head},"room":1,"crews":[2,1]}}"#), "as URLSearchParams asks");
    // (a + in a query is a space, as URLSearchParams reads it)
    assert_eq!(srv.get("/players?crews=4294967296,-8,8.0,+8,08").await.1, format!(r#"{head},"crews":[2]}}"#), "decimal u32s only: 08 is 8");
    assert_eq!(srv.get("/players?crews=x,0,").await.1, format!("{head}}}"), "none asked: no crews");
    assert_eq!(srv.get("/players?crews").await.1, format!("{head}}}"));
    b.close().await;
    assert_eq!(
        until("/players?crews=8,7", 1).await,
        r#"{"players":1,"online":0,"roam":1,"full":false,"queue":0,"roamBusy":false,"crews":[1,1]}"#,
        "Bob left"
    );
    assert_eq!(srv.stop().await, Some(0));
}

/// a client frame (masked with a zero mask: the payload as it is)
fn client_frame(op: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![0x80 | op];
    if payload.len() < 126 {
        f.push(0x80 | payload.len() as u8);
    } else {
        f.push(0x80 | 126);
        f.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    }
    f.extend_from_slice(&[0, 0, 0, 0]);
    f.extend_from_slice(payload);
    f
}

/// the text messages a server sent, out of what came after its 101 (unmasked frames), until `want` came or 3 s
async fn text_messages(s: &mut tokio::net::TcpStream, mut buf: Vec<u8>, want: &str) -> Vec<String> {
    let mut out = Vec::new();
    let end = Instant::now() + Duration::from_secs(3);
    loop {
        // whole frames in buf
        loop {
            if buf.len() < 2 {
                break;
            }
            let (op, n0) = (buf[0] & 15, (buf[1] & 127) as usize);
            let (n, at) = match n0 {
                126 if buf.len() >= 4 => (u16::from_be_bytes([buf[2], buf[3]]) as usize, 4),
                127 if buf.len() >= 10 => (u64::from_be_bytes(buf[2..10].try_into().unwrap()) as usize, 10),
                126 | 127 => break,
                n => (n, 2),
            };
            if buf.len() < at + n {
                break;
            }
            if op == 1 {
                out.push(String::from_utf8_lossy(&buf[at..at + n]).to_string());
            }
            buf.drain(..at + n);
        }
        if out.iter().any(|m| m.contains(want)) || Instant::now() > end {
            return out;
        }
        let mut tmp = [0u8; 4096];
        match tokio::time::timeout(Duration::from_millis(200), s.read(&mut tmp)).await {
            Ok(Ok(0)) | Ok(Err(_)) => return out,
            Ok(Ok(n)) => buf.extend_from_slice(&tmp[..n]),
            Err(_) => {}
        }
    }
}

/// the upgrade to /lan<query> and `frames` in one write (a client that does not wait for the 101): the text
/// messages the server sent back until `want` came
async fn pipelined(port: u16, query: &str, frames: &[u8], want: &str) -> Vec<String> {
    let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let mut q = format!(
        "GET /lan{query} HTTP/1.1\r\nHost: localhost:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
    )
    .into_bytes();
    q.extend_from_slice(frames);
    s.write_all(&q).await.unwrap();
    let mut buf = Vec::new();
    let mut tmp = [0u8; 4096];
    while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
        let n = s.read(&mut tmp).await.unwrap();
        assert!(n > 0, "the answer");
        buf.extend_from_slice(&tmp[..n]);
    }
    let end = buf.windows(4).position(|w| w == b"\r\n\r\n").unwrap() + 4;
    assert!(buf.starts_with(b"HTTP/1.1 101 "), "{}", String::from_utf8_lossy(&buf));
    text_messages(&mut s, buf[end..].to_vec(), want).await
}

// (the http crate's upgrade and the switch together) A client that sends its first messages in the same write as its
// handshake loses none of them: the hello names it; a resume's hello with its token gives it its address back, with no
// wait for the hello (the switch waits up to 5 s for it)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn messages_in_the_same_write_as_the_handshake_are_not_lost() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let (port, state) = (free_port(), tmp.path().join("state"));
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &[]), port);
    srv.ready().await;
    let hello = client_frame(1, br#"{"t":"hello","name":"Early","deltas":1}"#);
    let got = pipelined(port, "", &hello, r#""name":"Early""#).await;
    let welcome: Value = serde_json::from_str(got.iter().find(|m| m.contains("welcome")).expect("a welcome")).unwrap();
    assert!(got.iter().any(|m| m.contains(r#""t":"peers""#) && m.contains(r#""name":"Early""#)), "the hello named it: {got:?}");
    let (vip, token) = (welcome["vipStr"].as_str().unwrap().to_string(), welcome["token"].as_str().unwrap().to_string());
    // it leaves (no close frame: its address is kept for its token), and resumes in one write
    sleep(300).await;
    let t0 = Instant::now();
    let hello = client_frame(1, format!(r#"{{"t":"hello","name":"Early","vip":"{vip}","token":"{token}"}}"#).as_bytes());
    let got = pipelined(port, "?resume", &hello, "welcome").await;
    let back: Value = serde_json::from_str(got.iter().find(|m| m.contains("welcome")).expect("a welcome")).unwrap();
    assert_eq!(back["vipStr"], vip.as_str(), "its address back");
    assert!(t0.elapsed() < Duration::from_secs(2), "at once, not after the 5 s wait for a hello");
    assert_eq!(srv.stop().await, Some(0));
    let lines = srv.lines();
    assert!(lines.contains(&format!("[lan] {vip} is back (1 on the switch)")), "{lines:?}");
}

/// What the server did when a page replaced its connection while logged in to Online mode
#[derive(Debug)]
struct Replaced {
    /// the address came back to the new connection
    same_vip: bool,
    /// how the old connection ended (its close frame, or 1006 without one)
    old_closed: Option<(u16, String)>,
    /// the lobby's answer to a message on the game's lobby connection, now over the new WebSocket
    answer: Option<(String, String)>,
    players: String,
    console: Vec<String>,
}

async fn replaced(data: &Path, wasm: &Path, root: &Path) -> Replaced {
    let (port, state) = (free_port(), root.join("state"));
    let mut srv = Server::spawn(command(data, wasm, port, &state, &[]), port);
    srv.ready().await;
    let host = format!("localhost:{port}");
    let a = Browser::open_at(&host, port, "", None).await;
    a.hello("Ann");
    let c = a.login("Ann").await;
    // the page reconnects while its old connection is still on the switch (it died without a close)
    let b = Browser::resume(&host, port, "Ann", a.vip, &a.token).await;
    let old_closed = a.try_until(3000, |g| g.closed.clone()).await;
    b.adopt(c);
    b.send_msg(c, "move", &[("NAME", "Lounge".into())]);
    let answer = b.try_until(3000, |g| g.tcp[&c.port].last("move").map(|m| (m.kind.clone(), m.code.clone()))).await;
    let players = srv.players().await;
    assert_eq!(srv.stop().await, Some(0));
    let lines = srv.lines();
    let last = lines.iter().position(|l| l.starts_with("  access    :")).unwrap();
    let mut console: Vec<String> = lines[last + 1..].iter().filter(|l| !l.starts_with("game.wasm compressed")).cloned().collect();
    console.sort();
    Replaced { same_vip: b.vip == a.vip, old_closed, answer, players, console }
}

// (the switch's replacement of a connection and the Online host together) A page whose connection died without a
// close comes back with ?resume and its token while the old one is still on the switch: it takes the address over,
// the old connection goes, and the game's lobby connection goes on over the new one (the Online host never hears of
// a departure)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_page_that_replaces_its_connection_keeps_its_online_session() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let rust = replaced(&data, &wasm, tmp.path()).await;
    println!("{rust:#?}");
    assert!(rust.same_vip, "the address came back");
    assert_eq!(rust.old_closed, Some((1006, String::new())), "the old connection ended, no close frame (terminate)");
    assert_eq!(rust.answer, Some(("move".to_string(), String::new())), "the lobby answers over the new connection");
    assert_eq!(rust.players, r#"{"players":1,"online":1,"roam":0,"full":false,"queue":0,"roamBusy":false}"#);
    assert!(rust.console.contains(&"[lan] 10.77.0.2 is back (1 on the switch)".to_string()), "{:?}", rust.console);
    assert!(!rust.console.iter().any(|l| l.contains("the browser left")), "{:?}", rust.console);
}

/// `nfsu2-server ctl --state <state> <words>`: its exit code, and what it printed (stdout, else stderr)
fn ctl(state: &Path, words: &[&str]) -> (i32, String) {
    let out = Command::new(env!("CARGO_BIN_EXE_nfsu2-server")).arg("ctl").arg("--state").arg(state).args(words).output().unwrap();
    let text = String::from_utf8_lossy(if out.status.success() { &out.stdout } else { &out.stderr }).trim_end().to_string();
    (out.status.code().unwrap_or(-1), text)
}

/// a page from `xff` (its address, as a proxy on this machine names it) that says hello with page id `id`: what it
/// got by the time the switch closed it (or 2 s), and how
async fn turned_away(host: &str, port: u16, xff: &str, id: &str) -> (Vec<Value>, Option<(u16, String)>) {
    use futures_util::{SinkExt, StreamExt};
    use tokio_tungstenite::tungstenite::{Message, client::IntoClientRequest};
    let s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let mut q = format!("ws://{host}/lan").into_client_request().unwrap();
    q.headers_mut().insert("x-forwarded-for", xff.parse().unwrap());
    let (ws, _) = tokio_tungstenite::client_async(q, s).await.expect("the handshake");
    let (mut sink, mut stream) = ws.split();
    let hello = serde_json::json!({ "t": "hello", "name": "Ann", "id": id }).to_string();
    let _ = sink.send(Message::text(hello)).await;
    let (mut ctl, mut closed) = (Vec::new(), None);
    let _ = tokio::time::timeout(Duration::from_secs(2), async {
        while let Some(Ok(m)) = stream.next().await {
            match m {
                Message::Text(t) => ctl.push(serde_json::from_str(t.as_str()).unwrap()),
                Message::Close(f) => closed = Some(f.map(|f| (u16::from(f.code), f.reason.to_string())).unwrap_or((1005, String::new()))),
                _ => {}
            }
        }
    })
    .await;
    (ctl, closed)
}

/// the kinds of control messages it got, in order
fn kinds(ctl: &[Value]) -> Vec<String> {
    ctl.iter().map(|m| m["t"].as_str().unwrap_or("").to_string()).collect()
}

// The operator's commands (app/control.rs) on the running binary: who, a kick (the player is told, and may come
// back), a ban (told; turned away by its address, and by its page id at another address, which then is banned too;
// the other player is left be), bans, unban; the state folder keeps no address and no page id
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn kicks_and_bans_through_the_control_socket() {
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let (port, state) = (free_port(), tmp.path().join("state"));
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &["--online", "off", "--roam-hub", "off"]), port);
    srv.ready().await;
    let host = format!("localhost:{port}");
    let sock = state.join("control.sock").to_string_lossy().to_string();
    assert!(srv.lines().contains(&format!("  bans      : 0; nfsu2-server ctl who, kick, ban, unban, bans (through {sock})")), "{:?}", srv.lines());
    let hello = |name: &str, id: &str| serde_json::json!({ "t": "hello", "name": name, "id": id }).to_string();
    let ann = Browser::open_from(&host, port, "", None, None, Some("192.0.2.1")).await;
    ann.text(&hello("Ann", "page-ann"));
    let bob = Browser::open_from(&host, port, "", None, None, Some("192.0.2.2")).await;
    bob.text(&hello("Bob", "page-bob"));
    let (ann_vip, bob_vip) = (nfsu2_util::vip_str(ann.vip), nfsu2_util::vip_str(bob.vip));
    let t0 = Instant::now();
    let mut who = ctl(&state, &["who"]);
    while !who.1.contains("\"Bob\"") && t0.elapsed() < Duration::from_secs(3) {
        sleep(50).await;
        who = ctl(&state, &["who"]);
    }
    assert_eq!(who.0, 0, "{}", who.1);
    assert!(who.1.starts_with("2 on the switch\n"), "{}", who.1);
    assert!(who.1.contains(&format!("{ann_vip:<13}\"Ann\"")), "{}", who.1);

    // a kick: the message, the close; the player may come back
    let kick = ctl(&state, &["kick", "ann", "Stop", "ramming"]);
    assert_eq!(kick, (0, format!("kicked {ann_vip} \"Ann\"")));
    assert_eq!(ann.until_ms(3000, |g| g.closed.clone()).await, (4001, "kicked".into()));
    assert_eq!(ann.got().ctl.iter().find(|m| m["t"] == "kick").map(|m| m["msg"].clone()), Some(Value::from("Stop ramming")));
    let ann = Browser::open_from(&host, port, "", None, None, Some("192.0.2.1")).await;
    ann.text(&hello("Ann", "page-ann"));
    sleep(200).await;
    assert!(ann.is_open(), "a kick is no ban");

    // a ban: the same, and turned away
    let ann_vip = nfsu2_util::vip_str(ann.vip);
    let ban = ctl(&state, &["ban", &ann_vip, "3d", "Ramming,", "3", "days"]);
    assert_eq!(ban.0, 0, "{}", ban.1);
    assert!(ban.1.starts_with("ban #1: \"Ann\" for 3 d (until "), "{}", ban.1);
    assert!(ban.1.ends_with(&format!("by its page and its address\nremoved {ann_vip} \"Ann\"")), "{}", ban.1);
    assert_eq!(ann.until_ms(3000, |g| g.closed.clone()).await, (4003, "banned".into()));
    let told = ann.got().ctl.iter().find(|m| m["t"] == "ban").cloned().expect("the ban's message");
    assert_eq!(told["msg"], "Ramming, 3 days");
    let until = told["until"].as_i64().unwrap() - nfsu2_util::now_ms();
    assert!((3 * 86_400_000 - 10_000..=3 * 86_400_000).contains(&until), "{told}");
    // by its address (another page): before it gets a place
    let (got, closed) = turned_away(&host, port, "192.0.2.1", "page-other").await;
    assert_eq!((kinds(&got), closed), (vec!["ban".to_string()], Some((4003, "banned".to_string()))));
    // by its page at another address: at its hello, and that address is banned with it
    let (got, closed) = turned_away(&host, port, "192.0.2.9", "page-ann").await;
    assert_eq!((kinds(&got).last().cloned(), closed), (Some("ban".to_string()), Some((4003, "banned".to_string()))));
    let (got, _) = turned_away(&host, port, "192.0.2.9", "page-new").await;
    assert_eq!(kinds(&got), vec!["ban".to_string()], "the address the banned page came from");
    sleep(100).await;
    assert!(bob.is_open(), "Bob is left be");
    assert!(ctl(&state, &["who", "bob"]).1.starts_with("1 on the switch called *bob*\n"));

    let bans = ctl(&state, &["bans"]);
    assert_eq!(bans.0, 0);
    assert!(bans.1.starts_with("#1   \"Ann\""), "{}", bans.1);
    assert!(bans.1.ends_with(": \"Ramming, 3 days\"; by its page, 2 addresses"), "{}", bans.1);
    let file = std::fs::read_to_string(state.join("bans.json")).unwrap();
    assert!(!file.contains("192.0.2") && !file.contains("page-ann"), "{file}");

    // what cannot be done
    assert_eq!(ctl(&state, &["kick", "Nobody"]), (1, "nobody called \"Nobody\" on the switch or in the last hour (who lists them)".into()));
    assert_eq!(ctl(&state, &["kick", "Ann"]).0, 1, "Ann left");
    assert!(ctl(&state, &["ban", "Bob", "3 days"]).1.contains("how long"));
    assert_eq!(ctl(&state, &["unban", "#7"]), (1, "no ban #7 (bans lists them)".into()));
    assert_eq!(ctl(&tmp.path().join("elsewhere"), &["who"]).0, 1);

    // lifted: back in
    assert_eq!(ctl(&state, &["unban", "ann"]), (0, "lifted #1 \"Ann\"".into()));
    let ann = Browser::open_from(&host, port, "", None, None, Some("192.0.2.1")).await;
    ann.text(&hello("Ann", "page-ann"));
    sleep(200).await;
    assert!(ann.is_open());
    assert_eq!(ctl(&state, &["bans"]), (0, "no bans".into()));
    assert_eq!(bob_vip, nfsu2_util::vip_str(bob.vip));

    assert_eq!(srv.stop().await, Some(0));
    let lines = srv.lines();
    assert!(lines.iter().any(|l| l.starts_with("[ban] kicked 10.77.") && l.ends_with("\"Ann\": \"Stop ramming\"")), "{lines:?}");
    assert!(lines.iter().any(|l| l.starts_with("[ban] #1 \"Ann\" for 3 d (until ")), "{lines:?}");
    assert!(lines.iter().any(|l| l == "[ban] turned away a client of ban #1 \"Ann\" (its address)"), "{lines:?}");
    assert!(lines.iter().any(|l| l == "[ban] turned away a client of ban #1 \"Ann\" (its page)"), "{lines:?}");
    assert!(lines.iter().any(|l| l == "[ban] lifted #1 \"Ann\""), "{lines:?}");
    assert!(!lines.iter().any(|l| l.contains("192.0.2")), "no client address in the log: {lines:?}");
}

// The control socket's file (app/control.rs): a stale one, as a server that died without its stop leaves it, is
// replaced at the start; a SIGTERM takes it away.  Of two servers on one state folder (the next one started before
// the one before stops), the later one's socket stays when the earlier one stops.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_control_socket_goes_with_its_server() {
    use std::os::unix::fs::FileTypeExt;
    let tmp = tempfile::tempdir().unwrap();
    let (data, wasm) = game_folder(tmp.path());
    let state = tmp.path().join("state");
    let sock = state.join("control.sock");
    let socket_there = || std::fs::symlink_metadata(&sock).is_ok_and(|m| m.file_type().is_socket());
    // a socket nobody listens on any more
    std::fs::create_dir_all(&state).unwrap();
    drop(std::os::unix::net::UnixListener::bind(&sock).unwrap());
    assert!(socket_there());
    let port = free_port();
    let mut srv = Server::spawn(command(&data, &wasm, port, &state, &[]), port);
    srv.ready().await;
    assert_eq!(ctl(&state, &["bans"]), (0, "no bans".into()), "the stale socket replaced: {:?}", srv.lines());
    assert_eq!(srv.stop().await, Some(0));
    assert!(std::fs::symlink_metadata(&sock).is_err(), "SIGTERM: no control.sock left behind");
    assert_eq!(ctl(&state, &["bans"]).0, 1, "nobody there");

    let (pa, pb) = (free_port(), free_port());
    let mut a = Server::spawn(command(&data, &wasm, pa, &state, &[]), pa);
    a.ready().await;
    let mut b = Server::spawn(command(&data, &wasm, pb, &state, &[]), pb);
    b.ready().await;
    assert_eq!(a.stop().await, Some(0));
    assert!(socket_there(), "the later server's socket stays");
    assert_eq!(ctl(&state, &["bans"]), (0, "no bans".into()), "and answers");
    assert_eq!(b.stop().await, Some(0));
    assert!(std::fs::symlink_metadata(&sock).is_err(), "gone with the later server");
}
