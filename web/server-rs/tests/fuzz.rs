//! What a client can send the server's hosts, at random (a fixed seed), through the whole binary: lobby and EA
//! Messenger messages with random fields on real sessions, raw TCP, UDP and ICMP frames to 10.77.0.1, free-roam
//! datagrams to 10.77.0.253 (well formed with random contents, and garbage), and bundles of them, within the
//! switch's budgets.  Nothing of it may panic (the console would show it: the panic hook's lines, "[online] a service
//! failed"), and the server goes on: /players answers, a newcomer logs in, SIGTERM ends it with exit code 0 and
//! readable files.
//!   cargo test --test fuzz        (NFSU2_FUZZ_SEED, NFSU2_FUZZ_FRAMES: another seed, more frames)

mod common;

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use common::*;
use nfsu2_online::{PORT_BUDDY, PORT_HTTP, PORT_LOBBY, PORT_REDIRECTOR, VHOST_VIP};
use nfsu2_roam::ROAM_HUB_VIP;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

/// xorshift64*: the same frames for the same seed
struct Rng(std::cell::Cell<u64>);

impl Rng {
    fn next(&self) -> u64 {
        let mut x = self.0.get();
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0.set(x);
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    fn below(&self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
    fn chance(&self, pct: u64) -> bool {
        self.below(100) < pct
    }
    fn pick<'a, T>(&self, l: &'a [T]) -> &'a T {
        &l[self.below(l.len() as u64) as usize]
    }
    fn bytes(&self, n: usize) -> Vec<u8> {
        (0..n).map(|_| self.next() as u8).collect()
    }
}

const LOBBY_KINDS: &[&str] = &[
    "addr", "skey", "news", "sele", "~png", "auth", "acct", "lost", "cper", "dper", "pers", "move", "mesg", "auxi", "user", "onln", "gsea",
    "gcre", "gjoi", "gset", "gsta", "gdel", "glea", "rank", "snap", "rept", "zzzz", "@dir", "",
];
const BUDDY_KINDS: &[&str] = &[
    "AUTH", "EPGT", "RGET", "RADD", "RDEL", "RADM", "RRSP", "PADD", "PDEL", "USCH", "PSET", "SEND", "GINV", "GRSP", "GRVK", "DISC", "QQQQ",
];
const KEYS: &[&str] = &[
    "NAME",
    "PERS",
    "PASS",
    "ATTR",
    "AUTH",
    "CHAN",
    "COUNT",
    "CUSTFLAGS",
    "CUSTMASK",
    "FIND",
    "IDENT",
    "INDEX",
    "KICK",
    "MAXSIZE",
    "MINSIZE",
    "PARAMS",
    "PRIV",
    "RANGE",
    "RESU",
    "START",
    "STATS",
    "SYSFLAGS",
    "SYSMASK",
    "TEXT",
    "USERFLAGS",
    "NAME0",
    "NAME1",
    "NAME7",
    "USER",
    "LIST",
    "ID",
    "PROD",
    "SHOW",
    "STAT",
    "LKEY",
    "PRES",
    "VERS",
    "ADDR",
    "PORT",
    "SKEY",
    "MYGAME",
    "ASYNC",
    "MESGS",
    "TYPE",
    "BODY",
    "SECS",
    "ACPT",
    "X",
];

/// a field's value: numbers of every kind, names, escapes, long text, any byte
fn value(r: &Rng, names: &[&str]) -> String {
    match r.below(14) {
        0 => String::new(),
        1 => r.below(10).to_string(),
        2 => (r.next() as i64).to_string(),
        3 => r.pick(&["-1", "0", "1e308", "-0", "NaN", "Infinity", "0x10", "99999999999999999999999", "2147483648", "1.5"]).to_string(),
        4 => r.pick(names).to_string(),
        5 => format!("TRACK={}", r.below(5000)),
        6 => "A".repeat(r.below(3000) as usize),
        7 => r.bytes(r.below(40) as usize).iter().map(|&c| c as char).collect(),
        8 => "%zz%4%%41\"=:^ ".to_string(),
        9 => base64(&r.bytes(r.below(400) as usize)),
        10 => resu(r),
        11 => format!("{}={}", r.pick(KEYS), r.below(100)),
        12 => "\u{e9}\u{ff}\u{7f}".to_string(),
        _ => r.pick(&["Lounge", "Circuit", "Sprint", "Drift", "Drag", "StreetX", "URL", "B", "P", "C", "G"]).to_string(),
    }
}

fn base64(b: &[u8]) -> String {
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            s.push(if i <= c.len() { A[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' });
        }
    }
    s
}

/// a race report (rank RESU) that is nearly the game's: version 3, random counts, places, types and times
fn resu(r: &Rng) -> String {
    let n = r.below(10) as usize;
    let mut b = vec![0u8; 8 + 55 * n + r.below(3) as usize];
    b[0] = if r.chance(80) { 3 } else { r.next() as u8 };
    b[1] = if r.chance(80) { n as u8 } else { r.next() as u8 };
    b[2] = r.below(9) as u8;
    b[4..6].copy_from_slice(&(r.below(5000) as u16).to_be_bytes());
    b[7] = r.next() as u8;
    for i in 0..n {
        let o = 8 + 55 * i;
        b[o] = r.below(10) as u8;
        b[o + 1] = r.below(10) as u8;
        b[o + 2] = r.next() as u8;
        let t = match r.below(4) {
            0 => f32::NAN,
            1 => -1.0,
            2 => 1e30,
            _ => r.below(600) as f32,
        };
        b[o + 8..o + 12].copy_from_slice(&t.to_be_bytes());
    }
    base64(&b)
}

fn fields(r: &Rng, names: &[&str]) -> Vec<(&'static str, String)> {
    (0..r.below(6)).map(|_| (*r.pick(KEYS), value(r, names))).collect()
}

/// one free-roam datagram: a well-formed header and CRC around a random payload, a HELLO or SNAP with odd values, or
/// garbage
fn roam(r: &Rng) -> Vec<u8> {
    match r.below(5) {
        0 => {
            let mut p = r.bytes(r.below(120) as usize);
            if p.len() >= 2 && r.chance(50) {
                p[0..2].copy_from_slice(&(r.below(4) as u16).to_le_bytes());
            }
            dgram(r.below(12) as u8, r.next() as u32, &p)
        }
        1 => roam_hello(r.below(4) as u32),
        2 => {
            let v = [f32::NAN, f32::INFINITY, -1e9, 1e6, 0.0, 30000.0];
            roam_snap(r.below(4) as u32, [*r.pick(&v), *r.pick(&v), *r.pick(&v)])
        }
        3 => {
            // a HELLO whose name and list of seen players run past its end
            let mut h = roam_hello(r.next() as u32);
            let at = 16 + 12;
            h[at] = r.next() as u8;
            if r.chance(50) {
                h[at + 2] = 255;
            }
            let len = h.len();
            let crc = crc32(&h[..len - 4]);
            h[len - 4..].copy_from_slice(&crc.to_le_bytes());
            h
        }
        _ => r.bytes(r.below(80) as usize),
    }
}

/// a frame of any kind to anyone (the hosts most of all)
fn raw_frame(r: &Rng, others: &[u32]) -> Vec<u8> {
    let dst = match r.below(6) {
        0 | 1 => VHOST_VIP,
        2 | 3 => ROAM_HUB_VIP,
        4 => 0xFFFF_FFFF,
        _ => *r.pick(others),
    };
    let kind = *r.pick(&[1u8, 1, 3, 4, 4, 4, 5, 0, 2, 6, 31, 33, 255]);
    let dport = *r.pick(&[PORT_LOBBY, PORT_BUDDY, PORT_REDIRECTOR, PORT_HTTP, 47210, 1, 0, 65535]);
    let len = r.below(64) as usize;
    let mut f = lan_frame(kind, r.next() as u32, dst, r.next() as u16, dport, r.below(8) as u8, r.below(4) as u32, &r.bytes(len));
    if r.chance(10) {
        f.truncate(12 + r.below(13) as usize);
    }
    f
}

/// a bundle of up to 8 entries (destination lists of up to 3), now and then a broken one
fn bundle(r: &Rng, others: &[u32]) -> Vec<u8> {
    let mut b = vec![32, 0, 0, 0];
    for _ in 0..r.below(8) + 1 {
        let f = if r.chance(50) { udp(ROAM_HUB_VIP, 47210, 47210, 1, &roam(r)) } else { raw_frame(r, others) };
        let k = r.below(4) as usize;
        b.push(k as u8);
        b.push(0);
        let len = if r.chance(5) { r.next() as u16 } else { f.len() as u16 };
        b.extend_from_slice(&len.to_be_bytes());
        for _ in 0..k {
            let d = *r.pick(&[VHOST_VIP, ROAM_HUB_VIP, 0xFFFF_FFFF, *r.pick(others)]);
            b.extend_from_slice(&d.to_be_bytes());
        }
        b.extend_from_slice(&f);
    }
    b
}

async fn players(port: u16) -> String {
    let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let q = format!("GET /players HTTP/1.1\r\nHost: localhost:{port}\r\nConnection: close\r\n\r\n");
    s.write_all(q.as_bytes()).await.unwrap();
    let mut r = Vec::new();
    let _ = tokio::time::timeout(Duration::from_secs(5), s.read_to_end(&mut r)).await;
    String::from_utf8_lossy(&r).to_string()
}

/// a session of the fuzz: its browser, its lobby connection (logged in again once the lobby closed it) and its
/// Messenger connection
struct Player {
    b: Browser,
    name: String,
    lobby: Conn,
    buddy: Conn,
}

impl Player {
    async fn new(port: u16, name: &str) -> Player {
        let b = Browser::open_at(&format!("localhost:{port}"), port, "", None).await;
        b.hello(name);
        let (lobby, buddy) = Player::login(&b, name).await.expect("logged in");
        Player { b, name: name.into(), lobby, buddy }
    }

    /// a lobby session and a Messenger login with its key (None: the lobby or the virtual host refused one)
    async fn login(b: &Browser, name: &str) -> Option<(Conn, Conn)> {
        let lobby = b.try_login(name).await?;
        let lkey = b.tcp(lobby, |t| t.last("pers").and_then(|m| m.get("LKEY")).unwrap_or("").to_string());
        let buddy = b.try_open_tcp(PORT_BUDDY).await?;
        b.send_msg(
            buddy,
            "AUTH",
            &[
                ("PROD", "NFS-CONSOLE-2005".into()),
                ("VERS", "0.1".into()),
                ("PRES", "20920".into()),
                ("USER", "/PC/NFS-CONSOLE-2005".into()),
                ("LKEY", lkey),
            ],
        );
        Some((lobby, buddy))
    }

    /// the lobby or Messenger closed a connection (a flood, a bad message): both go (RST), and a new login (the
    /// next round tries again when the lobby refused it)
    async fn again_if_closed(&mut self) -> bool {
        let closed = |c: Conn| self.b.tcp(c, |t| t.reset || t.fin);
        if !closed(self.lobby) && !closed(self.buddy) {
            return false;
        }
        for c in [self.lobby, self.buddy] {
            self.b.tcp_out(c, RST, &[]);
        }
        if let Some((l, m)) = Player::login(&self.b, &self.name).await {
            self.lobby = l;
            self.buddy = m;
        }
        true
    }
}

/// a race between the two players (the first creates the game, the other joins, it starts), then race reports with
/// its AUTH, random results and names, and now and then the game's other messages
async fn race(ps: &[Player], r: &Rng, i: usize) {
    let (h, o) = if r.chance(50) { (&ps[0], &ps[1]) } else { (&ps[1], &ps[0]) };
    let name = format!("g{}", i % 7);
    let track = if r.chance(80) { "TRACK=4014".to_string() } else { value(r, &["Fuzz1"]) };
    h.b.send_msg(h.lobby, "gcre", &[("NAME", name.clone()), ("PARAMS", track), ("MAXSIZE", value(r, &["Fuzz1"]))]);
    o.b.send_msg(o.lobby, "gjoi", &[("NAME", name.clone())]);
    h.b.send_msg(h.lobby, "gsta", &[("NAME", name.clone())]);
    // (the AUTH of this race when it started in time, else of one before, else any)
    let auth =
        h.b.try_until(50, |g| g.tcp.get(&h.lobby.port).and_then(|t| t.last("+ses")).and_then(|m| m.get("AUTH")).map(str::to_string)).await;
    let auth = auth.unwrap_or_else(|| value(r, &["Fuzz1"]));
    for p in [h, o] {
        let mut f = vec![("AUTH", auth.clone()), ("RESU", resu(r))];
        let names = ["NAME0", "NAME1", "NAME2"];
        for n in names.iter().take(r.below(4) as usize) {
            f.push((n, r.pick(&[&p.name[..], "Fuzz1", "Fuzz2", "Nobody", ""]).to_string()));
        }
        p.b.send_msg(p.lobby, "rank", &f);
    }
    if r.chance(30) {
        let kind = *r.pick(&["gdel", "glea", "gset", "snap", "gsea"]);
        h.b.send_msg(h.lobby, kind, &fields(r, &["Fuzz1", "Fuzz2"]));
    }
}

/// What the server did under the fuzz
struct FuzzRun {
    /// its console, from the start-up lines on
    lines: Vec<String>,
    /// /players after the fuzz and once a newcomer logged in
    players: Vec<String>,
    exit: Option<i32>,
    logins: usize,
    /// the state folder's files after the stop
    files: Vec<(String, String)>,
}

/// the fuzz against the server
async fn fuzz(seed: u64, frames: usize) -> FuzzRun {
    let tmp = tempfile::tempdir().unwrap();
    let data = tmp.path().join("game");
    std::fs::create_dir_all(&data).unwrap();
    std::fs::write(data.join("SPEED2.EXE"), "MZ").unwrap();
    let state = tmp.path().join("state");
    let port = free_port();
    let mut child = Command::new(env!("CARGO_BIN_EXE_nfsu2-server"))
        .env("NFSU2_THREADS", "2")
        .env_remove("NFSU2_GAME")
        .arg("--data")
        .arg(&data)
        .args(["--http-port", &port.to_string(), "--state"])
        .arg(&state)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let out = Arc::new(Mutex::new(Vec::<String>::new()));
    for s in [Box::new(child.stdout.take().unwrap()) as Box<dyn std::io::Read + Send>, Box::new(child.stderr.take().unwrap())] {
        let out = out.clone();
        std::thread::spawn(move || {
            for l in BufReader::new(s).lines().map_while(Result::ok) {
                out.lock().unwrap().push(l);
            }
        });
    }
    assert!(
        wait_for(30_000, || out.lock().unwrap().iter().any(|l| l.starts_with("  access    :"))).await,
        "the server did not start: {:?}",
        out.lock().unwrap()
    );

    let mut ps = vec![Player::new(port, "Fuzz1").await, Player::new(port, "Fuzz2").await];
    // the raw frames come from a browser of their own: their SYNs hold the virtual host's connections for that
    // browser (8 at most) until the services' time-outs, and the players must be able to log in again
    let raw = Browser::open_at(&format!("localhost:{port}"), port, "", None).await;
    raw.hello("Raw");
    raw.got().drop_frames = true;
    let names = ["Fuzz1", "Fuzz2", "fuzz1", "Nobody", "", "Fuzz1 ", "F%41"];
    let others: Vec<u32> = ps.iter().map(|p| p.b.vip).chain([raw.vip]).collect();
    let r = Rng(std::cell::Cell::new(seed | 1));
    let mut logins = 0;
    for i in 0..frames {
        let p = &ps[r.below(2) as usize];
        let action = r.below(40);
        match action {
            0..=17 => {
                let kind = *r.pick(LOBBY_KINDS);
                let f = fields(&r, &names);
                p.b.send_msg(p.lobby, kind, &f);
            }
            18..=23 => {
                let kind = *r.pick(BUDDY_KINDS);
                let f = fields(&r, &names);
                p.b.send_msg(p.buddy, kind, &f);
            }
            24 | 25 => {
                // a lobby message whose header lies about its length, or DATA on a connection the host never opened
                let c =
                    if r.chance(50) { p.lobby } else { Conn { port: 1 + r.below(1000) as u16, cid: r.next() as u32, dport: PORT_LOBBY } };
                let mut m = encode_frame(r.pick(LOBBY_KINDS), &fields(&r, &names));
                let n = r.next() as u32;
                m[8..12].copy_from_slice(&n.to_be_bytes());
                p.b.tcp_out(c, DATA, &m);
            }
            26..=29 => p.b.binary(udp(ROAM_HUB_VIP, 47210, *r.pick(&[47210u16, 47211, 47212, 1]), 1, &roam(&r))),
            30 => race(&ps, &r, i).await,
            31..=34 => raw.binary(raw_frame(&r, &others)),
            _ => raw.binary(bundle(&r, &others)),
        }
        // about 1000 frames a second in all: within the budgets of one client address (5000 frames, 512 KiB)
        if i % 20 == 19 {
            p.b.flush().await;
            sleep(20).await;
            for p in ps.iter_mut() {
                logins += p.again_if_closed().await as usize;
            }
        }
    }
    sleep(500).await;
    // (a session the last frames closed: logged in again, as in the loop)
    for p in ps.iter_mut() {
        logins += p.again_if_closed().await as usize;
    }
    assert!(ps.iter().all(|p| p.b.is_open()) && raw.is_open(), "the fuzzing browsers are still on the switch");
    // the server goes on
    let mut players = vec![http_body(&self::players(port).await)];
    let c = Browser::open_at(&format!("localhost:{port}"), port, "", None).await;
    c.hello("Late");
    c.login("Late").await;
    players.push(http_body(&self::players(port).await));
    drop(ps);
    drop((raw, c));
    let _ = Command::new("kill").args(["-TERM", &child.id().to_string()]).status();
    let t = Instant::now();
    let exit = loop {
        if let Some(st) = child.try_wait().unwrap() {
            break st.code();
        }
        if t.elapsed() > Duration::from_secs(10) {
            let _ = child.kill();
            break None;
        }
        sleep(20).await;
    };
    sleep(100).await;
    let files = ["online/stats.json", "online/buddies.json", "lan-addresses.json"]
        .iter()
        .filter_map(|f| std::fs::read_to_string(state.join(f)).ok().map(|s| (f.to_string(), s)))
        .collect();
    let lines = out.lock().unwrap().clone();
    FuzzRun { lines, players, exit, logins, files }
}

fn http_body(r: &str) -> String {
    r.split_once("\r\n\r\n").map_or(r, |x| x.1).to_string()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn random_frames_to_the_hosts_panic_nothing_and_the_server_goes_on() {
    let seed: u64 = std::env::var("NFSU2_FUZZ_SEED").ok().and_then(|v| v.parse().ok()).unwrap_or(0x5EED_2026);
    let frames: usize = std::env::var("NFSU2_FUZZ_FRAMES").ok().and_then(|v| v.parse().ok()).unwrap_or(6000);
    let t0 = Instant::now();
    let run = fuzz(seed, frames).await;
    println!("{frames} frames in {:.1} s, {} logins again", t0.elapsed().as_secs_f64(), run.logins);
    println!("what the console said ({} lines):\n{}", run.lines.len(), run.lines.join("\n"));
    let bad: Vec<&String> = run.lines.iter().filter(|l| l.contains("panicked") || l.contains("a service failed")).collect();
    assert!(bad.is_empty(), "a panic: {bad:#?}");
    // /players: the three browsers, and one more with the newcomer, in Online mode too
    let p: Vec<serde_json::Value> = run.players.iter().map(|p| serde_json::from_str(p).unwrap()).collect();
    assert_eq!(p[0]["players"], 3, "{:?}", run.players);
    assert_eq!(p[1]["players"], 4, "{:?}", run.players);
    assert_eq!(p[1]["online"].as_u64(), p[0]["online"].as_u64().map(|n| n + 1), "{:?}", run.players);
    assert_eq!(run.exit, Some(0), "SIGTERM: exit code 0");
    for (f, s) in &run.files {
        assert!(serde_json::from_str::<serde_json::Value>(s).is_ok(), "{f}: {s}");
    }
}
